#pragma once

#include <cstdint>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "config_api.h"
#include "MotorDriver.h"  // for DriverConfig

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

    // Interpolator playback state (Core 1 only)
    float                  buf_seg_t          = 0.0f;
    uint32_t               buf_last_tick_us   = 0;
    volatile bool          buf_active         = false;  // emitting motion now?

    // ---- Loading / flow (cross-core) -----------------------------------------
    volatile bool          homed               = false;
    volatile bool          homing_in_progress  = false;
    bool                   wifi_ready          = false;   // Core 0 only

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

    // ---- Cadence / auto-duration ---------------------------------------------
    volatile bool          auto_duration        = true;
    uint32_t               last_cmd_ms          = 0;      // Core 1 only
    float                  measured_interval_ms = 0.0f;   // Core 1 only
    volatile uint16_t      measured_hz          = 0;      // written Core 1, read Core 0

    // ---- Default range (Core 0 only) -----------------------------------------
    float                  default_range_min = 0.0f;
    float                  default_range_max = PHYSICAL_MAX_TRAVEL_MM;

    // ---- On-device motion generator ------------------------------------------
    GeneratorConfig        gen;
    portMUX_TYPE           gen_mux     = portMUX_INITIALIZER_UNLOCKED;

    // Generator runtime phase/clock (Core 1 only — generatorTask)
    float                  gen_phase      = 0.0f;
    uint32_t               gen_last_us    = 0;
    float                  gen_mod_clock  = 0.0f;

    // Whether the generator is actually emitting motion (cross-core)
    volatile bool          gen_active      = false;

    // Timestamp of last Intiface command (Core 1 only — buttplugLinearCmd stamps)
    uint32_t               last_intiface_ms = 0;

    // Generator local tick rate (cross-core)
    volatile uint16_t      gen_rate_tick_hz = 50;

    // ---- Input mode & buffer tuning (cross-core) -----------------------------
    volatile uint8_t       input_mode   = static_cast<uint8_t>(InputMode::BUFFERED);
    volatile uint8_t       buf_easing   = 1;    // 0=linear 1=ease-in-out …
    volatile uint8_t       buf_depth    = 2;    // 1..5 samples of look-behind
    volatile uint16_t      buf_tick_hz  = 50;   // local interpolation rate (Hz)

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
};