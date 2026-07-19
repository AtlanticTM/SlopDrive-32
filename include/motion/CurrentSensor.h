#pragma once

// ============================================================================
// CurrentSensor — INA228 high-side current/voltage monitor on the 36V bus
// ============================================================================
//
// Build-guarded behind DRIVER_57AIM_SERVO. The custom v0.0 controller replaced
// the OSSM reference board's analog Hall sensor with a proper 20-bit INA228
// sitting on the DIRTY side behind an ISO1640 I2C isolator — but from the S3's
// point of view it's just a normal device on the Wire bus. Transparent. :3
//
// This is where the machine feels itself out. Every reading is the drive
// whispering back exactly how hard it's straining — free travel barely draws a
// trickle, but bury the carriage balls-deep against the hard stop and the
// current gushes toward the drive's ceiling. Sensorless homing lives on this.
//
// Wraps RobTillaart/INA228. Register map is NOT the INA226 — do not swap libs.
//   Address:   0x40 (A0/A1 -> GND)
//   Shunt:     5 mΩ, 2W
//   ADCRANGE:  0 -> ±163.84mV full scale -> 32.768 A full scale
//   SHUNT_CAL: 4096 (exact)
//   CURRENT_LSB: 62.5 µA
//   ALERT:     not wired — all limiting is polled in software. :3

#if defined(DRIVER_57AIM_SERVO)

#include <Arduino.h>

class CurrentSensor {
public:
    CurrentSensor() {}

    // Bring up the INA228 on the shared Wire bus. Wire.begin(SDA, SCL) MUST
    // already have been called by the caller (main setup) before this runs —
    // we don't own the bus, we just probe our device on it. Returns true if the
    // chip answers and calibrates cleanly, false if it's not found (motor
    // disconnected bring-up will still find it as long as the 36V rail is live).
    bool init();

    // True once init() succeeded. Homing must refuse to run if this is false —
    // no current source means no stall detection, and we won't blindly ram the
    // frame. :3
    bool isReady() const { return _ready; }

    // Instantaneous bus current in AMPS (signed — direction depends on wiring).
    // The homing loop watches the magnitude of this. Returns 0 if not ready. :3
    float readCurrentA();

    // Bus voltage in VOLTS — the actual 36V rail. Free telemetry, handy for the
    // bring-up sanity check (should read ~36V with the rail live). :3
    float readBusV();

    // Instantaneous bus power in WATTS (V×I product computed on-chip). Free —
    // the INA228 already has both operands, no extra math needed on our end. :3
    float readPowerW();

    // INA228 internal die temperature in °C — a free health signal. Not the
    // motor's temperature, just the sensor chip's, but a hot chip usually means
    // a hot board, which is worth knowing about before something lets go. :3
    float readDieTempC();

    // Raw shunt voltage in VOLTS — what the current measurement is actually
    // derived from. Mostly diagnostic (sanity-check the shunt/current math). :3
    float readShuntV();

    // Session energy in WATT-HOURS, read from the INA228's own 40-bit hardware
    // energy accumulator (it integrates continuously in the background, we
    // just read the running total). resetPeaks() clears it back to zero. :3
    float readEnergyWh();

    // ---- Cached, I2C-FREE accessors for cross-core telemetry ----------------
    // readCurrentA()/readBusV()/etc. do a live I2C transaction and are called
    // from the homing loop on Core 1. The WebUI status handler runs on Core 0
    // and must NOT hammer the same bus concurrently — so it reads these cached
    // copies instead, refreshed by whoever last polled the live registers.
    // Marked volatile: single 32-bit float loads/stores are atomic on the S3,
    // so a torn read is impossible and no mutex is needed for a display value. :3
    float cachedCurrentA() const { return _last_current_a; }
    float cachedBusV()     const { return _last_bus_v; }
    float cachedPowerW()   const { return _last_power_w; }
    float cachedDieTempC() const { return _last_die_temp_c; }
    float cachedShuntV()   const { return _last_shunt_v; }
    float cachedEnergyWh() const { return _last_energy_wh; }

    // ---- Peak tracking --------------------------------------------------------
    // Highest |current| / power seen since boot or since the last resetPeaks().
    // Updated automatically inside readCurrentA()/readPowerW() — no separate
    // poll path needed. Lets the operator see how hard the machine strained on
    // the last stroke/home without having to watch the live number live. :3
    float getPeakCurrentA() const { return _peak_current_a; }
    float getPeakPowerW()   const { return _peak_power_w; }

    // Clear the software peak-current/peak-power tracking AND the INA228's own
    // hardware energy/charge accumulators, so a fresh home/session starts the
    // Wh counter back at zero too. Call this at the start of a homing cycle. :3
    void  resetPeaks();

    // Refresh the cached copies WITHOUT anyone else on the bus. Safe to call
    // from Core 0 telemetry at a low rate (~1Hz) — the homing loop isn't running
    // then anyway (homing owns the machine exclusively while it runs). :3
    void  poll() {
        readCurrentA();
        readBusV();
        readPowerW();
        readDieTempC();
        readShuntV();
        readEnergyWh();
    }

    // Fast-path cache refresh — ONLY the two motion-diagnosis signals (bus
    // current + bus voltage), 2 I2C transactions (~0.3ms at 400kHz) instead
    // of poll()'s 6. Safe at 40Hz: matches the chip's own conversion cadence
    // (540µs conversions × AVG16 ≈ one fresh conversion every ~26ms), so
    // polling faster than this would just re-read the same conversion. The
    // hardware 16-sample averaging is what keeps the signal clean — the
    // refresh RATE and the noise floor are decoupled on this chip. :3
    void  pollFast() {
        readCurrentA();
        readBusV();
    }

private:
    bool _ready = false;
    // Last live readings, updated on every read*() call. The WebUI sips these
    // without touching I2C. :3
    volatile float _last_current_a  = 0.0f;
    volatile float _last_bus_v      = 0.0f;
    volatile float _last_power_w    = 0.0f;
    volatile float _last_die_temp_c = 0.0f;
    volatile float _last_shunt_v    = 0.0f;
    volatile float _last_energy_wh  = 0.0f;

    // Peak trackers — highest magnitude seen since boot / since resetPeaks(). :3
    volatile float _peak_current_a = 0.0f;
    volatile float _peak_power_w   = 0.0f;
};


#endif // defined(DRIVER_57AIM_SERVO)
